"""Measure text contrast by anchoring on KNOWN backgrounds, not by guessing glyphs.

The previous attempt (audit_contrast.py) tried to identify glyph pixels heuristically and
failed loudly: it reported 16/16 pages as bad because 1px borders and icon art looked like
short strokes. That tool is not usable for a verdict and is not wired into any gate.

This version inverts the approach. The theme has a small, known set of surface colours.
For each page and each known surface, take the pixels of that surface and look at what
NON-surface colours appear strictly inside it. Those are the glyphs sitting on it. The
surface colour is then the true background by construction, so the contrast number is
meaningful.

Limitations are stated in the output rather than hidden:
  * only the surfaces in SURFACES below are examined; a control with a bespoke fill is
    not covered,
  * icon interiors are indistinguishable from text by colour alone, so a pair is only
    reported when it covers at least MIN_PX pixels of the surface, which icon specks do
    not reach on a flat field.
"""

from __future__ import annotations

import collections
import glob
import os
import sys

LIGHT_SURFACES = {
    "Surface": (0xFF, 0xFF, 0xFF),
    "SurfaceAlt": (0xEF, 0xF6, 0xFC),
    "HeaderBand": (0x9B, 0xC7, 0x16),
    "Accent": (0x00, 0x7A, 0xCC),
}
DARK_SURFACES = {
    "Surface": (0x1F, 0x1F, 0x1F),
    "SurfaceAlt": (0x2B, 0x2B, 0x2B),
    "HeaderBand": (0x4E, 0x6B, 0x08),
    "Accent": (0x00, 0x7A, 0xCC),
}

TOL = 3
MIN_PX = 120        # a glyph run on a flat field clears this easily; icon specks do not
THRESHOLD = 4.5


def lin(c: float) -> float:
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(rgb) -> float:
    r, g, b = rgb
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a, b) -> float:
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def near(c, t, tol=TOL):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))


def analyse(path, surfaces, step=1):
    from PIL import Image

    im = Image.open(path).convert("RGB")
    w, h = im.size
    px = im.load()

    out = []
    for name, bg in surfaces.items():
        # pixels of this surface, and what colour shows up adjacent to them
        field = collections.Counter()
        fg = collections.Counter()
        for y in range(0, h, step):
            for x in range(0, w, step):
                if near(px[x, y], bg):
                    field[(x, y)] = True
        if len(field) < 5000:          # surface not present on this page
            continue
        # A pixel only counts as "text ON this surface" if the surface surrounds it on
        # BOTH sides horizontally. Requiring just one neighbour was what produced the
        # false positives: at the boundary between Surface (#1F1F1F) and SurfaceAlt
        # (#2B2B2B) each looked like "text" on the other, reporting ratio 1.16 on all
        # 16 pages. A glyph stroke has background on both sides; a boundary has it on
        # one side only.
        for (x, y) in field:
            lx = x - 2
            rx = x + 2
            if not (0 <= lx and rx < w):
                continue
            if not (near(px[lx, y], bg) and near(px[rx, y], bg)):
                continue
            c = px[x, y]
            if not near(c, bg):
                fg[c] += 1
        for c, n in fg.items():
            if n < MIN_PX:
                continue
            r = contrast(c, bg)
            if r < THRESHOLD:
                out.append((name, c, bg, r, n))
    out.sort(key=lambda t: -t[4])
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print("[FAIL] usage: contrast_known_bg.py <dir> <light|dark>")
        return 2
    d, theme = sys.argv[1], sys.argv[2]
    surfaces = DARK_SURFACES if theme == "dark" else LIGHT_SURFACES

    shots = sorted(glob.glob(os.path.join(d, "*.bmp")))
    if not shots:
        print("[FAIL] no captures under " + d)
        return 2

    print("[INFO] theme=%s pages=%d surfaces=%d"
          % (theme, len(shots), len(surfaces)))
    bad = 0
    for p in shots:
        res = analyse(p, surfaces)
        # collapse: one line per (surface, fg)
        seen = set()
        lines = []
        for sname, c, bg, r, n in res:
            k = (sname, c)
            if k in seen:
                continue
            seen.add(k)
            lines.append((sname, c, bg, r, n))
        if not lines:
            continue
        bad += 1
        print("%-18s %d sub-%.1f pair(s)" % (os.path.basename(p), len(lines), THRESHOLD))
        for sname, c, bg, r, n in lines[:4]:
            print("      on %-11s text #%02X%02X%02X  ratio=%.2f  px=%d"
                  % (sname, c[0], c[1], c[2], r, n))

    print("\n[RESULT] pages with a sub-%.1f:1 pair on a known surface: %d / %d"
          % (THRESHOLD, bad, len(shots)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
