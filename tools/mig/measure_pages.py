"""UNUSABLE -- kept for the record, do not wire into any gate.

Measured nav_w = 575 where the true value is 233: it treated the white content
field as "same colour as the nav background" and kept extending. The quantities it
targets (content start, row pitch) cannot be identified from pixels alone; more
heuristics would only fail differently. Use compare_layout.py instead.

--- original docstring below ---

Measure page geometry from screenshots, in logical pixels, for both trees.

Why a single tool with one method
---------------------------------
Every previous comparison used ad-hoc scripts, and two of them produced wrong verdicts by
confusing icons or borders with structure. The fix is not a cleverer heuristic -- it is to
measure the SAME well-defined quantities on both images with the SAME code, then compare
numbers. Anchor points are chosen to be robust:

  nav_right      the vertical separator between the navigation pane and content: the
                 first column, to the right of the nav, whose colour differs from the
                 nav background for most of the content height
  content_left   first column right of nav_right that is NOT the page background
                 (i.e. where the page's own content starts)
  flow_top       first row below the header block where the content area changes colour
                 (the top of the first list/table)
  row_pitch      median distance between repeated horizontal separator lines in the list

All four are computed from long runs, not single pixels, so a stray icon cannot move them.

Usage:
    measure_pages.py <image-dir> <scale>            # scale = width / 1280
    measure_pages.py <image-dir> <scale> --json
"""

from __future__ import annotations

import glob
import os
import statistics
import sys


def load(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im, im.load(), im.size[0], im.size[1]


def same(a, b, tol=4):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def col_histogram(px, w, h, x, step=2):
    """Most common colour down a column -- the column's "background"."""
    from collections import Counter
    c = Counter(px[x, y] for y in range(0, h, step))
    return c.most_common(1)[0][0]


def find_nav_right(px, w, h, step=2):
    """Rightmost column of the nav pane: scan for a long vertical colour change."""
    from collections import Counter
    # The nav is a flat field on the left. Find the first x where the column's dominant
    # colour differs from x=2's dominant colour for good.
    base = col_histogram(px, w, h, min(2, w - 1), step)
    last_same = 0
    for x in range(0, min(w, int(w * 0.45))):
        c = col_histogram(px, w, h, x, step)
        if same(c, base, 6):
            last_same = x
        elif x - last_same > 8:
            return last_same
    return last_same


def find_content_left(px, w, h, nav_right, step=2):
    """First column past the nav whose dominant colour is not the page background."""
    from collections import Counter
    bg = col_histogram(px, w, h, min(nav_right + 6, w - 1), step)
    for x in range(nav_right + 1, w):
        c = col_histogram(px, w, h, x, step)
        if not same(c, bg, 6):
            return x
    return nav_right + 1


def find_flow_top(px, w, h, x_probe, step=2):
    """First row below the header where a long horizontal run changes colour."""
    from collections import Counter
    # Skip the blue header block: find the last row that is mostly the header colour.
    hdr = Counter(px[x, y] for x in range(0, w, 8) for y in range(0, min(h, int(h*0.3)), 4))
    hdr_col = hdr.most_common(1)[0][0]
    last_hdr = 0
    for y in range(0, min(h, int(h * 0.35))):
        c = Counter(px[x, y] for x in range(0, w, 8)).most_common(1)[0][0]
        if same(c, hdr_col, 8):
            last_hdr = y
    return last_hdr + 1


def list_row_pitch(px, w, h, x0, x1, y0, y1, step=1):
    """Median gap between rows that are mostly a single non-background colour (separators)."""
    from collections import Counter
    rows = []
    xs = list(range(x0, x1, 3))
    if not xs:
        return None
    for y in range(y0, min(y1, h)):
        # most_common(1) on an empty sequence returns [], which crashed the first version
        # on pages whose content area is narrower than the probe window.
        hist = Counter(px[x, y] for x in xs).most_common(1)
        if not hist:
            continue
        col, n = hist[0]
        rows.append((y, col, n / len(xs)))
    # a separator row: one colour covers >70% and differs from the list background
    seps = [y for y, col, frac in rows if frac > 0.70]
    if len(seps) < 4:
        return None
    gaps = [b - a for a, b in zip(seps, seps[1:]) if 0 < b - a < h]
    return statistics.median(gaps) if gaps else None


def measure(path, scale):
    im, px, w, h = load(path)
    nav = find_nav_right(px, w, h)
    left = find_content_left(px, w, h, nav)
    top = find_flow_top(px, w, h, max(nav + 4, w // 2))
    pitch = list_row_pitch(px, w, h, left + 4, w - 4, top, int(h * 0.95))
    return {
        "size": (w, h),
        "nav_w": round(nav / scale, 1),
        "content_left": round(left / scale, 1),
        "flow_top": round(top / scale, 1),
        "row_pitch": round(pitch / scale, 1) if pitch else None,
    }


def main() -> int:
    if len(sys.argv) < 3:
        print("[FAIL] usage: measure_pages.py <dir> <scale>")
        return 2
    d = sys.argv[1]
    scale = float(sys.argv[2])
    files = sorted(glob.glob(os.path.join(d, "*.bmp")) + glob.glob(os.path.join(d, "*.png")))
    if not files:
        print("[FAIL] no images under " + d)
        return 2
    print("[INFO] dir=%s scale=%.4f files=%d" % (d, scale, len(files)))
    print("%-22s %-8s %-9s %-9s %-9s" % ("page", "nav_w", "cont_left", "flow_top", "row_pitch"))
    for f in files:
        m = measure(f, scale)
        print("%-22s %-8s %-9s %-9s %-9s"
              % (os.path.basename(f)[:22], m["nav_w"], m["content_left"],
                 m["flow_top"], m["row_pitch"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
