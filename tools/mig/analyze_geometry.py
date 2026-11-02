"""Report button geometry from the control-tree dump, classified by role.

Why this supersedes the pixel approach
--------------------------------------
Twelve rounds tried to measure button widths from screenshots. That worked for the bottom
bar (104 logical px, consistent across 12 of 13 reference shots) but could not separate a
window-close button from a menu item from a content button -- they all look like "a
rectangle of some width" to a pixel scan.

The control tree has names. `minbtn`/`maxbtn`/`closebtn` are window controls; `menu_*` are
menu items; everything else is content. Classifying by name is exact, and it turned a
confusing "widths 46/74/82/84/98/104 are all present" into three clean groups.

Usage:
    analyze_geometry.py <dir-with-_geometry.txt.NN.txt>

Exit: 0 report produced, 2 no dumps found.
"""

from __future__ import annotations

import collections
import glob
import os
import sys

# Capture width / logical width. The dumps come from a 2240px-wide render of a 1280px
# window, so geometry is stored in capture pixels and reported in logical ones.
SCALE = 2240.0 / 1280.0

WINDOW_CTL = ("minbtn", "maxbtn", "closebtn", "restorebtn")
MENU = ("menu_",)

# The reference design uses one size for content buttons; the tool reports the
# distribution so a deviation is visible rather than being averaged away.
EXPECTED_CONTENT_W = 104


def role_of(name: str) -> str:
    for k in WINDOW_CTL:
        if name.startswith(k):
            return "window-control"
    for k in MENU:
        if name.startswith(k):
            return "menu"
    return "content"


def load_dumps(directory: str):
    files = sorted(glob.glob(os.path.join(directory, "_geometry.txt.*.txt")))
    if not files:
        files = sorted(glob.glob(os.path.join(directory, "_geometry.txt")))
    return files


def main() -> int:
    if len(sys.argv) < 2:
        print("[FAIL] usage: analyze_geometry.py <dir>")
        return 2
    files = load_dumps(sys.argv[1])
    if not files:
        print("[FAIL] no _geometry.txt dumps under " + sys.argv[1])
        return 2

    print("[INFO] dumps=%d scale=%.3f" % (len(files), SCALE))

    per_page = {}
    for f in files:
        page = os.path.basename(f).replace("_geometry.txt.", "").replace(".txt", "")
        groups = collections.defaultdict(collections.Counter)
        controls = 0
        for line in open(f, encoding="utf-8", errors="replace"):
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if len(p) != 8:
                continue
            controls += 1
            name, cls, l, t, r, b, vis = p[1], p[2], p[3], p[4], p[5], p[6], p[7]
            if cls != "ButtonUI" or vis != "1":
                continue
            w = int(r) - int(l)
            if w <= 0:
                continue
            groups[role_of(name)][round(w / SCALE)] += 1
        per_page[page] = (controls, groups)

    # Overall view first: this is what tells a reader whether one width dominates.
    total = collections.defaultdict(collections.Counter)
    for _c, groups in per_page.values():
        for role, cnt in groups.items():
            total[role].update(cnt)

    print("\n=== button widths by role (all pages) ===")
    for role in ("window-control", "menu", "content"):
        cnt = total.get(role)
        if not cnt:
            continue
        print("  %-15s %-4d widths=%s"
              % (role, sum(cnt.values()), cnt.most_common(5)))

    # Content buttons are the ones the design specifies, so check them explicitly.
    content = total.get("content", collections.Counter())
    if not content:
        print("\n[FAIL] no visible content buttons found -- the dump is not usable")
        return 2
    off = {w: n for w, n in content.items() if w != EXPECTED_CONTENT_W}
    if off:
        print("\n[INFO] content buttons not at %d logical px: %s"
              % (EXPECTED_CONTENT_W, sorted(off.items(), key=lambda kv: -kv[1])[:6]))
        print("       these live in card grids / title bars and need per-page review;")
        print("       they are reported, not failed, because several are legitimate.")
    main_w = content.most_common(1)[0][0]
    print("\n[MODE] content button width = %d logical px (%d of %d)"
          % (main_w, content[main_w], sum(content.values())))
    print("[PASS] geometry report produced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
