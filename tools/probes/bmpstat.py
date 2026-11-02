#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Minimal 24/32-bit BMP reader + colour statistics for theme-residue checks.

No PIL / numpy: parses the BITMAPFILEHEADER + BITMAPINFOHEADER by hand.
BMP rows are bottom-up and padded to 4 bytes, which is handled below.
"""
import struct
import sys
from collections import Counter


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP: " + path)
    offset = struct.unpack_from("<I", data, 10)[0]
    hdr_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp not in (24, 32):
        raise ValueError("unsupported bpp %d" % bpp)
    flip = height > 0
    height = abs(height)
    stride = ((width * bpp + 31) // 32) * 4
    step = bpp // 8

    def pixel(x, y):
        row = (height - 1 - y) if flip else y
        i = offset + row * stride + x * step
        b, g, r = data[i], data[i + 1], data[i + 2]
        return (r, g, b)

    return width, height, pixel


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def hexc(c):
    return "%02X%02X%02X" % c


def main():
    if len(sys.argv) < 3:
        print("usage: bmpstat.py <a.bmp> <b.bmp> [mode]")
        return 2
    pa, pb = sys.argv[1], sys.argv[2]
    wa, ha, pixa = read_bmp(pa)
    wb, hb, pixb = read_bmp(pb)
    if (wa, ha) != (wb, hb):
        print("size mismatch: %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 2

    total = 0
    changed = 0
    bright_in_a = 0      # near-white pixels in image A
    bright_in_b = 0
    unchanged_bright = 0  # bright in both -> possible residue if A is dark theme
    for y in range(0, ha, 3):
        for x in range(0, wa, 3):
            ca = pixa(x, y)
            cb = pixb(x, y)
            total += 1
            if ca != cb:
                changed += 1
            ba = lum(ca) > 200
            bb = lum(cb) > 200
            if ba:
                bright_in_a += 1
            if bb:
                bright_in_b += 1
            if ba and bb:
                unchanged_bright += 1

    print("size          : %dx%d" % (wa, ha))
    print("sampled px    : %d" % total)
    print("changed       : %d (%.1f%%)" % (changed, 100.0 * changed / total))
    print("bright in A   : %d (%.2f%%)" % (bright_in_a, 100.0 * bright_in_a / total))
    print("bright in B   : %d (%.2f%%)" % (bright_in_b, 100.0 * bright_in_b / total))
    print("bright in both: %d (%.2f%%)  <- same white in both themes" %
          (unchanged_bright, 100.0 * unchanged_bright / total))

    # Dominant colours of a horizontal band, useful for header-band checks.
    if len(sys.argv) > 3 and sys.argv[3] == "rows":
        for frac in (0.22, 0.23, 0.24):
            y = int(ha * frac)
            cnt = Counter(hexc(pixa(x, y)) for x in range(0, wa, 5))
            print("row y=%d (%.2f) A: %s" % (y, frac, cnt.most_common(3)))
            cnt = Counter(hexc(pixb(x, y)) for x in range(0, wa, 5))
            print("row y=%d (%.2f) B: %s" % (y, frac, cnt.most_common(3)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
