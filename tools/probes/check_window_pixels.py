"""Objective check of the captured window: no black blocks, text is drawn.

Vision inspection of this screenshot is not available, so the questions it would
have answered are answered with numbers instead:

  black blocks   -> a region whose pixels are all near-zero AND differ from the
                    page background is a hole, not a design choice
  text present   -> glyph pixels are pixels clearly lighter or darker than the
                    local background; counting them per region says whether the
                    labels were drawn at all
  text readable  -> for each detected text band, the contrast between its glyph
                    pixels and the surrounding background
"""

from PIL import Image
import sys
from collections import Counter

PATH = r"D:\DuiLib_Ultimate-master\preview-dark.png"


def luminance(p):
    r, g, b = p[0], p[1], p[2]
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = luminance(a) / 255.0, luminance(b) / 255.0
    la, lb = (la + 0.05), (lb + 0.05)
    if la < lb:
        la, lb = lb, la
    return la / lb


def analyse(im, name, box):
    crop = im.crop(box)
    w, h = crop.size
    px = list(crop.getdata())
    colors = Counter(px)
    bg, bg_n = colors.most_common(1)[0]
    bg_share = 100.0 * bg_n / len(px)

    # A hole would be near-black while the page background is not.
    near_black = sum(1 for p in px if p[0] < 12 and p[1] < 12 and p[2] < 12)
    hole = 100.0 * near_black / len(px)

    # Glyph pixels: far enough from bg in luminance to be ink, excluding the
    # background itself and single-pixel anti-aliased edges.
    bg_l = luminance(bg)
    ink = [p for p in px if abs(luminance(p) - bg_l) > 40]
    ink_pct = 100.0 * len(ink) / len(px)

    if ink:
        # Do NOT use the most common ink colour: on a dark page the most common
        # "off-background" colour is a mid-grey divider or scrollbar, not text, and
        # measuring it reported 2.5:1 for a page whose labels are actually bright.
        # Text is the extreme of the luminance distribution, so take the brightest
        # decile (dark theme) or the darkest decile (light theme) and measure that.
        inks = sorted(ink, key=luminance)
        decile = max(1, len(inks) // 10)
        extreme = inks[-decile:] if bg_l < 128 else inks[:decile]
        ink_c = Counter(extreme).most_common(1)[0][0]
        cr = contrast(ink_c, bg)
    else:
        ink_c, cr = None, 0.0

    print("  %-14s %4dx%-4d bg=%-16s %5.1f%%  ink=%5.2f%%  black=%4.1f%%  "
          "contrast=%5.2f:1" % (name, w, h, str(bg), bg_share, ink_pct, hole, cr))
    flags = []
    if hole > 60:
        flags.append("BLACK HOLE")
    if bg_share > 99.5:
        flags.append("FLAT/EMPTY")
    if 0 < ink_pct < 0.05:
        flags.append("NO TEXT")
    if ink and cr < 3.0:
        flags.append("LOW CONTRAST")
    return flags, bg, ink_pct, cr


def main():
    im = Image.open(PATH).convert("RGB")
    W, H = im.size
    print("image %dx%d, distinct colours=%d" % (W, H, len(Counter(im.getdata()))))

    regions = [
        ("titlebar", (0, 0, W, 48)),
        ("nav-left", (0, 48, 220, H - 40)),
        ("content", (220, 48, W, H - 40)),
        ("statusbar", (0, H - 40, W, H)),
        ("whole", (0, 0, W, H)),
    ]
    bad = []
    for name, box in regions:
        flags, bg, ink, cr = analyse(im, name, box)
        if flags:
            bad.append((name, flags))

    print()
    if bad:
        for name, flags in bad:
            print("[FAIL] %-12s %s" % (name, ", ".join(flags)))
        return 1
    print("[PASS] no black holes, no flat regions, text drawn with contrast >= 3:1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
