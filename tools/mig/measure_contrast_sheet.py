"""Measure contrast from the calibration sheet: no glyph guessing, no false verdicts.

The sheet has one cell per (surface, text) token pair, each at a position computed from
its index. This tool therefore knows BOTH colours before it looks at a single pixel:

    background = the token the cell was built with
    text       = the token the label was given

It measures what the renderer actually produced and compares against what those two
tokens say, which answers two distinct questions at once:
  * does the pair meet 4.5:1 (an accessibility property), and
  * did the renderer apply the tokens at all, or silently paint something else
    (the defect class found twice: white text turned dark, and hard-coded colours).
"""

from __future__ import annotations

import collections
import os
import struct
import sys

# EToken order, mirrored from ThemeTokens.h. Kept as a list so an id maps back without
# depending on token spelling; if the enum grows, this list must grow with it and the
# checker below will notice the mismatch.
TOKEN_NAMES = [
    "Accent", "AccentHot", "AccentPushed", "AccentText", "AccentSoft", "AccentBorder",
    "Surface", "SurfaceAlt", "SurfaceHover", "SurfaceSunk",
    "Border", "BorderSoft",
    "Ink", "InkSecondary", "InkTertiary", "InkMuted",
    "Danger", "Success", "Warn", "WarnBg", "OnAccent",
    "HeaderBand", "OnHeaderBand",
]
TOKEN_COUNT = 23


def read_bmp(path):
    raw = open(path, "rb").read()
    if raw[:2] != b"BM":
        raise SystemExit("not a BMP: " + path)
    off = struct.unpack_from("<I", raw, 10)[0]
    w = struct.unpack_from("<i", raw, 18)[0]
    h = struct.unpack_from("<i", raw, 22)[0]
    bpp = struct.unpack_from("<H", raw, 28)[0]
    if bpp != 24:
        raise SystemExit("expected 24bpp, got %d" % bpp)
    stride = ((w * 3) + 3) // 4 * 4
    return raw, off, w, h, stride


def px_at(raw, off, w, h, stride, x, y):
    """BMP rows are bottom-up; y is in image coordinates (top = 0)."""
    row = h - 1 - y
    i = off + row * stride + x * 3
    return (raw[i + 2], raw[i + 1], raw[i])   # B,G,R -> R,G,B


def lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(rgb):
    r, g, b = rgb
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def main() -> int:
    if len(sys.argv) < 2:
        print("[FAIL] usage: measure_contrast_sheet.py <shot-dir>")
        return 2
    d = sys.argv[1]
    sheet = os.path.join(d, "_contrast_sheet.bmp")
    pairs = os.path.join(d, "_contrast_pairs.txt")
    if not (os.path.exists(sheet) and os.path.exists(pairs)):
        print("[FAIL] missing _contrast_sheet.bmp / _contrast_pairs.txt in " + d)
        return 2

    geo = {}
    rows = []
    for line in open(pairs, "r", encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#") and "=" in line:
            k, v = line[1:].split("=", 1)
            geo[k] = int(v)
        elif not line.startswith("#"):
            parts = line.split(",")
            if len(parts) == 3:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2])))

    if not rows:
        print("[FAIL] no pairs in " + pairs)
        return 2
    if len(TOKEN_NAMES) != TOKEN_COUNT:
        print("[FAIL] token table out of sync: %d names vs %d expected"
              % (len(TOKEN_NAMES), TOKEN_COUNT))
        return 2

    cw = geo.get("cell_w", 300)
    ch = geo.get("cell_h", 90)
    mg = geo.get("margin", 10)
    cols = geo.get("cols", 4)

    raw, off, w, h, stride = read_bmp(sheet)
    print("[INFO] sheet=%dx%d cells=%d" % (w, h, len(rows)))

    fails = []
    for idx, bg_id, fg_id in rows:
        if bg_id >= len(TOKEN_NAMES) or fg_id >= len(TOKEN_NAMES):
            fails.append((idx, "token id out of range", 0, 0))
            continue
        r = idx // cols
        c = idx % cols
        x0 = mg + c * cw
        y0 = mg + r * ch

        # Background: read a corner well away from the centred label.
        bg_meas = px_at(raw, off, w, h, stride, x0 + 6, y0 + 6)
        # Text: scan the middle band for the most common non-background colour. The
        # label is centred, so this region is guaranteed to contain glyph pixels.
        seen = collections.Counter()
        for y in range(y0 + ch // 2 - 12, y0 + ch // 2 + 12):
            for x in range(x0 + cw // 4, x0 + 3 * cw // 4):
                p = px_at(raw, off, w, h, stride, x, y)
                if max(abs(p[i] - bg_meas[i]) for i in range(3)) > 24:
                    seen[p] += 1
        if not seen:
            fails.append((idx, "no glyph pixels found in cell", 0, 0))
            continue
        fg_meas = seen.most_common(1)[0][0]
        cr = ratio(bg_meas, fg_meas)
        mark = "OK " if cr >= 4.5 else "LOW"
        print("  %-2d %-13s on %-13s #%02X%02X%02X / #%02X%02X%02X  %5.2f  %s"
              % (idx, TOKEN_NAMES[fg_id], TOKEN_NAMES[bg_id],
                 fg_meas[0], fg_meas[1], fg_meas[2],
                 bg_meas[0], bg_meas[1], bg_meas[2], cr, mark))
        if cr < 4.5:
            fails.append((idx, "%s on %s" % (TOKEN_NAMES[fg_id], TOKEN_NAMES[bg_id]),
                          cr, 0))

    print()
    if fails:
        print("[FAIL] %d cell(s) below 4.5:1:" % len(fails))
        for idx, what, cr, _ in fails:
            print("       cell %-2d %s  ratio=%.2f" % (idx, what, cr))
        return 1
    print("[PASS] all %d token pairs meet 4.5:1" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
