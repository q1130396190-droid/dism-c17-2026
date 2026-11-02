#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Downscale 24/32-bit BMP to PNG without PIL / numpy.

Why this exists: the page screenshots exported by `/SHOTSWITCH` are 2240x1575
24-bit BMP (~10 MB each). The image viewer refuses files that large, so they
have to be shrunk before they can be inspected. There is no PIL in this
environment and `Add-Type` (which would give System.Drawing) is blocked, so the
BMP reader and the PNG writer are both implemented here with the stdlib only.

Usage:
    python bmp2png.py <in.bmp> <out.png> [scale]
        scale defaults to 2 (halve both dimensions, nearest neighbour)
"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:2] != b"BM":
        raise ValueError("not a BMP: " + path)
    offset = struct.unpack_from("<I", d, 10)[0]
    width = struct.unpack_from("<i", d, 18)[0]
    height = struct.unpack_from("<i", d, 22)[0]
    bpp = struct.unpack_from("<H", d, 28)[0]
    if bpp not in (24, 32):
        raise ValueError("unsupported bpp %d" % bpp)
    flip = height > 0
    height = abs(height)
    stride = ((width * bpp + 31) // 32) * 4
    step = bpp // 8

    def pixel(x, y):
        row = (height - 1 - y) if flip else y
        i = offset + row * stride + x * step
        return d[i + 2], d[i + 1], d[i]     # BGR -> RGB

    return width, height, pixel


def write_png(path, width, height, rows):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 3:
        print("usage: bmp2png.py <in.bmp> <out.png> [scale]")
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 2

    w, h, pixel = read_bmp(src)
    ow, oh = w // scale, h // scale
    rows = []
    for y in range(oh):
        sy = y * scale
        row = bytearray()
        for x in range(ow):
            r, g, b = pixel(x * scale, sy)
            row += bytes((r, g, b))
        rows.append(row)
    write_png(dst, ow, oh, rows)
    print("%s -> %s  %dx%d (from %dx%d)" % (src, dst, ow, oh, w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
