"""Assert the green header band renders exactly on the three pages the design specifies.

Design source (measured, not assumed): of the 15 reference screenshots, a strict scan for
"rows where green covers >50% of the width" finds exactly 4 shots, covering 3 pages:

    3756  Appx          tab band      87px @1729 canvas
    3758  System opt    table header  35px
    3760  Program       table header  35px
    272   System opt    table header  35px   (same page, newer build)

The other 11 shots have no band. So the correct state is exactly three pages green and
thirteen not. Any other count is a regression in one of two directions:
  * too many  -> the token leaked into containers that should stay neutral
  * too few   -> the token resolved to nothing (e.g. missing from kTokenNames[]) and the
                 band silently painted black-on-nothing

Both are invisible in a skin diff, which is why this reads pixels.

Self-test: --inject plants a fake expectation that must fail, proving the checker can
report. A gate that cannot fail is not a gate.
"""

from __future__ import annotations

import glob
import os
import sys

GREEN_LIGHT = (0x9B, 0xC7, 0x16)   # reference value, measured from the screenshots
GREEN_DARK = (0x4E, 0x6B, 0x08)    # dark-theme variant used by OnHeaderBand pairing
TOL = 4

# page keyword -> expected band height range in the 2240px-wide captures
EXPECTED = {
    "Appx": (95, 125),      # tab band, 62 logical px
    "Optim": (38, 56),      # table header, 26 logical px
    "Program": (38, 56),
}


def near(c, t, tol=TOL):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))


def band_rows(px, w, h, colour):
    """Rows where `colour` covers more than 40% of the width.

    Strided sampling (every 4th pixel, every 3rd row) rather than every pixel: a band
    is tens of rows tall, so missing a row cannot hide it, and the check drops from
    ~52s to a couple of seconds over 16 large captures. Correctness is preserved because
    the decision is "is this row mostly green", not "count exactly".
    """
    rows = []
    step_x = 4
    samples = len(range(0, w, step_x))
    threshold = samples * 0.4
    for y in range(0, h, 3):
        hit = sum(1 for x in range(0, w, step_x) if near(px[x, y], colour))
        if hit > threshold:
            rows.append(y)
    return rows


def scan(directory: str):
    """Return {basename: band_height or 0} using whichever green matches the theme."""
    from PIL import Image

    out = {}
    for p in sorted(glob.glob(os.path.join(directory, "*.bmp"))):
        im = Image.open(p).convert("RGB")
        w, h = im.size
        px = im.load()
        rows = band_rows(px, w, h, GREEN_LIGHT)
        if not rows:
            rows = band_rows(px, w, h, GREEN_DARK)
        out[os.path.basename(p)] = (max(rows) - min(rows) + 1) if rows else 0
    return out


def verdict(result):
    """Return (ok, problems) applying the "exactly three pages" rule."""
    problems = []
    green_pages = [n for n, bh in result.items() if bh > 0]

    for name, bh in result.items():
        key = next((k for k in EXPECTED if k in name), None)
        if key:
            lo, hi = EXPECTED[key]
            if bh == 0:
                problems.append("%s: band missing (expected %d-%d px)" % (name, lo, hi))
            elif not (lo <= bh <= hi):
                problems.append("%s: band height %d outside %d-%d px"
                                % (name, bh, lo, hi))
        else:
            if bh > 0:
                problems.append("%s: unexpected green band (%d px) -- token leaked"
                                % (name, bh))

    if len(green_pages) != len(EXPECTED):
        problems.append("page count %d with a band, expected %d"
                        % (len(green_pages), len(EXPECTED)))
    return (not problems), problems


def main() -> int:
    if len(sys.argv) < 2:
        print("[FAIL] usage: check_green_band.py <shot-dir> [--inject]")
        return 2
    directory = sys.argv[1]
    inject = "--inject" in sys.argv[2:]

    if not os.path.isdir(directory):
        print("[FAIL] no such directory: " + directory)
        return 2

    result = scan(directory)
    if not result:
        print("[FAIL] zero captures in %s -- nothing can pass this way" % directory)
        return 2

    print("[INFO] pages=%d  dir=%s" % (len(result), directory))

    if inject:
        # Flip one expectation so the rule must report: this proves the checker reads
        # enough to distinguish right from wrong state.
        saved = dict(EXPECTED)
        try:
            EXPECTED["Optim"] = (9999, 9999)   # impossible height
            ok, problems = verdict(result)
        finally:
            EXPECTED.clear()
            EXPECTED.update(saved)
        if ok or not problems:
            print("[FAIL] self-test: perturbed expectation still passed -- checker is blind")
            return 3
        print("[PASS] self-test: perturbed expectation reported %d problem(s)" % len(problems))
        return 0

    ok, problems = verdict(result)
    for name, bh in sorted(result.items()):
        mark = "GREEN %dpx" % bh if bh else "-"
        print("       %-18s %s" % (name, mark))

    if ok:
        print("[PASS] green band on exactly %d page(s), heights in range" % len(EXPECTED))
        return 0

    print("[FAIL] %d problem(s):" % len(problems))
    for p in problems:
        print("       " + p)
    return 1


if __name__ == "__main__":
    sys.exit(main())
