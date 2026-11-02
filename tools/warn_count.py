"""Count and classify MSVC warnings for each tree's Win8Metro build.

Why this is a tool and not a one-off script
-------------------------------------------
Documents in docs/ quote specific numbers ("main repo 92, all C4251; arch tree
0"). Those numbers are only useful if someone can re-derive them. A number
written into prose ages into a claim; a number with a command attached stays a
fact.

Design notes
------------
* No hard-coded SDK or toolset version: both are read from the machine. The
  project has already been bitten by a stale hard-coded SDK path.
* --no-build by default? No: warnings only exist after compiling. But --reuse
  lets you re-summarise the log from the previous run instead of paying for a
  rebuild when you only want the classification.
* Writes the raw log next to the report so a surprising count can be traced to
  actual compiler output rather than trusted blindly.
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys

VCROOT_CANDIDATES = [
    r"C:\Program Files\Microsoft Visual Studio\18\Community",
    r"C:\Program Files\Microsoft Visual Studio\18\Enterprise",
    r"C:\Program Files\Microsoft Visual Studio\18\Professional",
]


def find_msbuild():
    for root in VCROOT_CANDIDATES:
        p = os.path.join(root, r"MSBuild\Current\Bin\MSBuild.exe")
        if os.path.exists(p):
            return p
    # last resort: newest VS installation with MSBuild
    for base in (r"C:\Program Files\Microsoft Visual Studio", r"C:\Program Files (x86)\Microsoft Visual Studio"):
        if not os.path.isdir(base):
            continue
        for ed in sorted(os.listdir(base), reverse=True):
            p = os.path.join(base, ed, r"MSBuild\Current\Bin\MSBuild.exe")
            if os.path.exists(p):
                return p
    return None


def find_sdk():
    cands = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Include\*"))
    return os.path.basename(cands[-1]) if cands else None


def find_toolset(msbuild):
    """Newest MSVC toolset present, e.g. v145."""
    base = os.path.join(os.path.dirname(os.path.dirname(msbuild)),
                        "Microsoft.Cpp", "v4.0")
    # Simpler heuristic: look for v14x dirs under VC\Tools\MSVC and map the
    # first digits to a toolset name; fall back to v145 which this repo uses.
    vc = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(msbuild))), "VC")
    tools = os.path.join(vc, "Tools", "MSVC")
    if os.path.isdir(tools):
        vers = sorted(os.listdir(tools), reverse=True)
        if vers:
            major, minor = vers[0].split(".")[:2]
            # 14.51 -> v145 (MSVC 19.5x is v145 in VS2026 naming)
            if major == "14" and minor.isdigit():
                return "v14" + minor[0]
    return "v145"


def build(root, tag, logdir):
    msbuild = find_msbuild()
    if not msbuild:
        raise SystemExit("MSBuild not found")
    sdk = find_sdk()
    toolset = find_toolset(msbuild)

    proj = os.path.join(root, r"Demos\Win8Metro\Win8Metro.vcxproj")
    if not os.path.exists(proj):
        raise SystemExit("project not found: " + proj)

    args = [msbuild, proj, "/t:Rebuild",
            "/p:Configuration=Release", "/p:Platform=x64",
            "/p:PlatformToolset=" + toolset,
            "/p:SolutionDir=" + root + os.sep,
            "/m", "/nologo", "/v:minimal"]
    if sdk:
        args.append("/p:WindowsTargetPlatformVersion=" + sdk)

    r = subprocess.run(args, cwd=root, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    out = (r.stdout or "") + (r.stderr or "")
    logpath = os.path.join(logdir, os.path.basename(os.path.normpath(root)),
                           "warn_%s.log" % tag)
    os.makedirs(os.path.dirname(logpath), exist_ok=True)
    with open(logpath, "w", encoding="utf-8") as f:
        f.write(out)
    return out, r.returncode, logpath


def classify(out):
    codes = re.findall(r"warning (C\d+)", out)
    return collections.Counter(codes), len(codes)


def summarise(tag, counter, total, logpath):
    print("=== %s === total warnings=%d" % (tag, total))
    for k, v in counter.most_common(12):
        print("   %-8s %d" % (k, v))
    print("   raw log: %s" % logpath)
    return counter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="repo root to build (default: this repo)")
    ap.add_argument("--compare", default="",
                    help="second repo root to build and diff against")
    ap.add_argument("--logdir", default="", help="where to write raw logs")
    ap.add_argument("--reuse", action="store_true",
                    help="re-summarise existing logs instead of rebuilding")
    args = ap.parse_args()

    # Logs live under each repo by default. Sharing one directory between two
    # trees let a --reuse run in the arch tree pick up the main tree's log and
    # report numbers that belonged to the other repo.
    logdir = args.logdir or os.path.join(os.path.abspath(args.root), "Temp", "warn-logs")

    results = {}
    # Tag by repo name, not a fixed "main"/"compare": with --reuse and a shared
    # logdir, a fixed tag made the arch tree silently read the main tree's log
    # and report 92 warnings that belonged to the other repo.
    def tag_of(path):
        """Stable, collision-free label for a repo root.

        Using the bare directory name looked fine until "DuiLib_Ultimate-master"
        and "DuiLib_Ultimate-master架构版" both mapped to log names that did not
        match what --reuse expects. Keep the two roles explicit instead.
        """
        base = os.path.basename(os.path.normpath(path))
        if base == "DuiLib_Ultimate-master":
            return "main"
        if base.startswith("DuiLib_Ultimate-master"):
            return "arch"
        return base or "repo"

    pairs = []
    if args.root:
        pairs.append((tag_of(args.root), args.root))
    if args.compare:
        pairs.append((tag_of(args.compare), args.compare))

    for tag, root in pairs:
        if not root:
            continue
        if args.reuse:
            # per-root subdirectory so the two trees can never read each other
            logpath = os.path.join(logdir, os.path.basename(os.path.normpath(root)),
                                    "warn_%s.log" % tag)
            if not os.path.exists(logpath):
                raise SystemExit("no log to reuse: " + logpath)
            out = open(logpath, encoding="utf-8", errors="replace").read()
        else:
            out, rc, logpath = build(root, tag, logdir)
            if rc != 0:
                print("NOTE: build exited %d -- warnings may be from a failed build" % rc)
        counter, total = classify(out)
        results[tag] = summarise(tag, counter, total, logpath)
        print()

    if len(results) == 2:
        ks = list(results)
        c1, c2 = results[ks[0]], results[ks[1]]
        print("=== difference (code: main vs compare) ===")
        any_diff = False
        for code in sorted(set(c1) | set(c2)):
            a, b = c1.get(code, 0), c2.get(code, 0)
            if a != b:
                print("   %-8s %-6d %-6d" % (code, a, b))
                any_diff = True
        if not any_diff:
            print("   none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
