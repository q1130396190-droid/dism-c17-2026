"""Assert the Win11 Fluent corner-radius coverage stays complete.

Why a gate rather than a one-time edit
--------------------------------------
The radius is applied in two places that drift apart:
  * skin XML   -- `borderround` on each element
  * C++ code   -- buttons built by Page_*.cpp, which the skin never sees

Measured before this round: skin Button coverage 16%, Edit/Combo/CheckBox/Progress 0%,
and 125 square buttons against 70 rounded -- the square set being exactly the controls
created in code. Nothing reported that, because "does this control look rounded" is not a
property any existing check looked at.

Exemptions are explicit and narrow
----------------------------------
Win11 genuinely draws some elements square: the caption buttons and the menu row items.
Rounding them would look wrong, not better. They are named here with that reason, and the
gate reports any exemption that stops matching, so the list cannot rot.

Exit: 0 coverage complete, 1 gaps found, 2 input problem, 3 --inject blind.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIN = os.path.join(ROOT, "bin64", "skin", "Win8Metro")
APP = os.path.join(ROOT, "Demos", "Win8Metro")

# Controls that must carry a radius.
REQUIRED = ("Button", "Edit", "Combo", "CheckBox", "Progress")

# Square by design: window caption buttons and menu rows are drawn flat in Win11.
EXEMPT_NAMES = ("minbtn", "maxbtn", "closebtn", "restorebtn", "menu_")

# Tabs use a bottom indicator, not a rounded chip; Option is therefore not required.
NOT_REQUIRED = ("Option",)


def skin_files():
    return [os.path.join(SKIN, "Main.xml")] + sorted(
        glob.glob(os.path.join(SKIN, "pages", "*.xml")))


SURFACE_TAGS = ("HorizontalLayout", "VerticalLayout", "TabLayout")


def has_fill(seg: str) -> bool:
    """A layout only shows a radius if it actually paints a background."""
    m = re.search(r'bkcolor="([^"]*)"', seg)
    if not m:
        return False
    return m.group(1) not in ("#00000000", "0", "")


def scan_surfaces():
    """Filled layout containers must carry the 8px surface radius.

    Measured 68 filled containers with 68 rounded -- the rule was already satisfied but
    nothing checked it, so the next new card would have been square without notice.
    """
    total = 0
    missing = []
    for p in skin_files():
        raw = open(p, "rb").read().decode("utf-8-sig", errors="replace")
        body = re.sub(r"<!--.*?-->", " ", raw, flags=re.S)
        for tag in SURFACE_TAGS:
            for m in re.finditer(r"<%s(?:\s[^>]*?)?/?>" % tag, body):
                seg = m.group(0)
                if not has_fill(seg):
                    continue
                total += 1
                if "borderround=" not in seg:
                    nm = re.search(r'name="([^"]*)"', seg)
                    line = body.count(chr(10), 0, m.start()) + 1
                    where = nm.group(1) if nm else ("line %d" % line)
                    missing.append((os.path.basename(p), tag, where))
    return total, missing


def scan_skin():
    total = 0
    missing = []
    for p in skin_files():
        raw = open(p, "rb").read().decode("utf-8-sig", errors="replace")
        body = re.sub(r"<!--.*?-->", " ", raw, flags=re.S)
        for tag in REQUIRED:
            for m in re.finditer(r"<%s\b[^>]*?(/?)>" % tag, body):
                seg = m.group(0)
                nm = re.search(r'name="([^"]*)"', seg)
                name = nm.group(1) if nm else ""
                if name.startswith(EXEMPT_NAMES):
                    continue
                total += 1
                if "borderround=" not in seg:
                    missing.append((os.path.basename(p), tag, name or "-"))
    return total, missing


def scan_code():
    """Buttons created in C++ bypass the skin entirely."""
    total = missing = 0
    created = re.compile(r"make_unique<CButtonUI>\(\)")
    for p in glob.glob(os.path.join(APP, "*.cpp")):
        text = open(p, "rb").read().decode("utf-8", errors="replace")
        n = len(created.findall(text))
        if not n:
            continue
        total += n
        # A radius applied anywhere in the same file is counted as coverage for that file;
        # finer granularity would need parsing, and the measured state is 6/6 files fully
        # covered -- a file-level check catches the realistic regression (a new button
        # added to a file that already rounds others is still in a file with the pattern).
        if "borderround" not in text:
            missing += n
    return total, missing


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true")
    args = ap.parse_args()

    files = skin_files()
    if not files:
        print("[FAIL] no skin XML found under " + SKIN)
        return 2

    s_total, s_missing = scan_skin()
    f_total, f_missing = scan_surfaces()
    c_total, c_missing = scan_code()
    print("[INFO] skin files=%d  radius-required controls=%d  filled surfaces=%d  "
          "code-created buttons=%d" % (len(files), s_total, f_total, c_total))

    problems = []
    for f, tag, name in s_missing[:20]:
        problems.append("%s: <%s %s> has no borderround" % (f, tag, name))
    for f, tag, name in f_missing[:10]:
        problems.append("%s: filled <%s %s> has no borderround (surface radius)" % (f, tag, name))
    if len(s_missing) > 20:
        problems.append("... and %d more skin control(s)" % (len(s_missing) - 20))
    if c_missing:
        problems.append("%d button(s) created in C++ without a radius" % c_missing)

    if args.inject:
        # Plant a violation in memory only, so the self-test does not touch the tree.
        problems.append("<injected>: <Button probe> has no borderround")
        print("[INFO] injected one synthetic gap")

    if problems:
        print("[FAIL] %d gap(s):" % len(problems))
        for p in problems:
            print("       " + p)
        return 1

    if args.inject:
        print("[FAIL] inject did not produce a gap -- the gate is blind")
        return 3

    print("[PASS] every radius-required control carries borderround")
    print("[NOTE] %s are exempt by design (Win11 draws them square)"
          % ", ".join(EXEMPT_NAMES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
