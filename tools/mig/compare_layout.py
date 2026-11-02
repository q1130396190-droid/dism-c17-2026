"""Compare the layout skeleton of page screenshots between the two trees.

A previous attempt tried to name every element (nav width, content start, row pitch) with
heuristics, and it produced nav_w = 575 when the true value is 233 -- it mistook the white
content field for "same as nav background". That tool is deleted, not fixed: the quantity
it was reaching for cannot be identified reliably from pixels alone.

What IS reliable is a separator line: a column whose colour is not the page background and
which repeats down most of the window height. There is exactly one such line between the
navigation pane and the content, so finding it is unambiguous.

This tool therefore checks two things only, both well-defined:
  * the navigation/content boundary, in logical pixels
  * whether the whole image is structurally similar after scale normalisation (reported as
    the fraction of sampled pixels whose row-dominant colour matches)

Exit: 0 within tolerance, 1 a mismatch beyond tolerance, 2 input problem.
"""

from __future__ import annotations

import collections
import glob
import os
import sys

TOLERANCE = 3.0     # logical px; the two screenshots were captured at 1.351 and 1.75


def load(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.load(), im.size[0], im.size[1]


def nav_boundary(path, scale):
    """x of the vertical separator between nav and content, in logical px."""
    px, w, h = load(path)
    best_n, best_x = 0, None
    for x in range(int(w * 0.05), int(w * 0.45)):
        c = collections.Counter(px[x, y] for y in range(int(h * 0.05), int(h * 0.90), 3))
        col, n = c.most_common(1)[0]
        if col == (255, 255, 255) or col == (0x1F, 0x1F, 0x1F):
            continue          # page background, not a separator
        if n > best_n:
            best_n, best_x = n, x
    if best_x is None:
        return None
    return round(best_x / scale, 1)


def row_signature(path, scale, rows=64):
    """Dominant colour of N evenly spaced rows, as a coarse layout fingerprint."""
    px, w, h = load(path)
    sig = []
    for i in range(rows):
        y = int((i + 0.5) * h / rows)
        c = collections.Counter(px[x, y] for x in range(0, w, 4)).most_common(1)[0][0]
        sig.append(c)
    return sig


def main() -> int:
    if len(sys.argv) < 4:
        print("[FAIL] usage: compare_layout.py <ref.png> <ref_scale> <cur.bmp> <cur_scale>")
        return 2
    ref, rs, cur, cs = sys.argv[1], float(sys.argv[2]), sys.argv[3], float(sys.argv[4])
    if not (os.path.exists(ref) and os.path.exists(cur)):
        print("[FAIL] missing input")
        return 2

    rb = nav_boundary(ref, rs)
    cb = nav_boundary(cur, cs)
    print("[INFO] ref=%s cur=%s" % (os.path.basename(ref), os.path.basename(cur)))
    print("       nav boundary  ref=%s  cur=%s" % (rb, cb))

    bad = 0
    if rb is None or cb is None:
        print("[FAIL] could not locate the nav separator in one of the images")
        bad += 1
    else:
        d = abs(rb - cb)
        ok = d <= TOLERANCE
        print("       delta=%.1f logical px  %s" % (d, "OK" if ok else "MISMATCH"))
        if not ok:
            bad += 1

    # Coarse structural similarity: how many of the sampled rows agree on their dominant
    # colour. This is deliberately weak -- it catches a page rendered with a different
    # theme or a wholly different layout, not a 4px padding change.
    r_sig = row_signature(ref, rs)
    c_sig = row_signature(cur, cs)
    agree = sum(1 for a, b in zip(r_sig, c_sig) if a == b)
    print("       row-dominant agreement: %d/%d" % (agree, len(r_sig)))
    if agree < len(r_sig) * 0.6:
        print("[FAIL] layout differs structurally (row colours disagree on %d rows)"
              % (len(r_sig) - agree))
        bad += 1

    if bad:
        print("[FAIL] %d check(s) failed" % bad)
        return 1
    print("[PASS] nav boundary within %.1f px and structure agrees" % TOLERANCE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
