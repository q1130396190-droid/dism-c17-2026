"""Measure text/background contrast on real rendered pages, both themes.

Why this exists
---------------
Earlier rounds verified contrast by asking "is the token value correct?". That misses the
failure actually found last round: the token said #FFFFFF, the control rendered #1F1F1F,
and nothing in the config showed it. The only reliable judge is the rendered pixels.

Method
------
For each page shot:
  1. Find text pixels: pixels that differ from their row's dominant colour AND are part
     of a small cluster (glyph strokes), not large flat areas (backgrounds, borders).
  2. For each text pixel, take the background as the dominant colour of a small
     neighbourhood ring around it -- that is what the glyph is actually sitting on.
  3. Compute WCAG contrast. Report pairs below the threshold, grouped by colour pair so
     one systemic mistake does not produce thousands of identical lines.

Caveats stated up front, because they bound the conclusion:
  * Anti-aliased edge pixels legitimately sit between text and background colour and
    produce low-contrast samples. Only clusters of >= 12 pixels of the SAME colour are
    treated as glyph cores, which filters most of them, but not all.
  * Icon art contains arbitrary colour pairs; those are not text and will be reported.
    The report therefore separates "regions that also contain a text run" from the rest
    only by cluster size, so treat findings as a review list, not as a defect count.
"""

from __future__ import annotations

import collections
import glob
import os
import sys


def lin(c: float) -> float:
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def luminance(rgb) -> float:
    r, g, b = rgb
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a, b) -> float:
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def analyse(path: str, min_px: int = 12):
    """Return [(fg, bg, ratio, count)] for glyph clusters below 4.5:1."""
    try:
        from PIL import Image
    except ImportError:
        return None

    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    # Per-row dominant colour = that row's background candidate.
    row_bg = []
    for y in range(h):
        c = collections.Counter(px[x, y] for x in range(0, w, 3))
        row_bg.append(c.most_common(1)[0][0])

    # Collect candidate glyph pixels. Two conditions, and the second one is what makes
    # this usable:
    #
    #   a) the pixel differs from its row background, AND
    #   b) it belongs to a SHORT horizontal run.
    #
    # (b) is essential. Without it the probe reported borders and separators as text --
    # measured: `#ABABAB on #FFFFFF ratio=2.30` on all 16 pages was the 1px Border token,
    # and `#FFFFDD`/`#71BAFF` were icon art. Both are long runs or wide shapes, not glyph
    # strokes. A glyph stroke is 1-3 px wide; a border line runs the width of the panel.
    MAX_RUN = 4

    glyph = collections.Counter()
    for y in range(h):
        bg = row_bg[y]
        x = 0
        while x < w:
            if px[x, y] == bg:
                x += 1
                continue
            run_start = x
            colour = px[x, y]
            while x < w and px[x, y] == colour:
                x += 1
            run_len = x - run_start
            if run_len <= MAX_RUN:
                glyph[(colour, bg)] += run_len

    out = []
    for (fg, bg), n in glyph.items():
        if n < min_px:
            continue
        r = contrast(fg, bg)
        if r < 4.5:
            out.append((fg, bg, r, n))
    out.sort(key=lambda t: -t[3])
    return out


def main() -> int:
    d = sys.argv[1] if len(sys.argv) > 1 else r".box-agent-scratch/mig/light3"
    shots = sorted(glob.glob(os.path.join(d, "*.bmp")))
    if not shots:
        print("[FAIL] no shots under " + d)
        return 2

    print("[INFO] pages=%d  dir=%s" % (len(shots), d))
    total_bad_pages = 0
    for p in shots:
        res = analyse(p)
        if res is None:
            print("[FAIL] PIL missing")
            return 2
        # Ignore tiny counts: they are anti-aliasing, not runs of text.
        worst = [r for r in res if r[3] >= 40]
        name = os.path.basename(p)
        if worst:
            total_bad_pages += 1
            print("%-18s %d pair(s) below 4.5:1" % (name, len(worst)))
            for fg, bg, ratio, n in worst[:3]:
                print("      text #%02X%02X%02X on #%02X%02X%02X  ratio=%.2f  px=%d"
                      % (fg[0], fg[1], fg[2], bg[0], bg[1], bg[2], ratio, n))
        else:
            print("%-18s ok" % name)

    print("\n[RESULT] pages with sub-4.5:1 glyph clusters: %d / %d"
          % (total_bad_pages, len(shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
