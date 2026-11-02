"""Verify the accent-derived tokens stay readable for ANY system accent colour.

Why this needs its own check
----------------------------
`AccentText` and `OnAccent` are not fixed values: they are computed at runtime from
whatever accent colour Windows gives us. The previous round could only check them by
substituting the Windows default, and noted that as a gap -- "user picks a dark accent,
do we still clear 4.5:1?"

Two algorithms are involved, and each needs a different argument:

  FitAccentForText(accent)  moves the colour toward white (dark theme) or black (light
                            theme) in up to 20 steps of 10% until it clears 4.5:1 against
                            the worst surface it can land on. It can only guarantee this
                            if 20 steps are enough for every starting colour.

  OnAccent                  picks black or white by luminance, i.e. whichever of the two
                            gives the higher ratio. That is optimal by construction, but
                            "optimal of two" is only 4.5:1 if the accent's luminance is
                            not in the middle band where both are mediocre.

Rather than reason about those bands, this sweeps the colour space and measures. The sweep
replicates the algorithms exactly, so a change to either one shows up here as a failure.

Exit: 0 all pass, 1 any failing sample, 2 input problem.
"""

from __future__ import annotations

import colorsys
import sys

# Surfaces the accent text can land on, mirrored from ThemeTokens.cpp.
DARK_TEXT_BG = (0x2B, 0x2B, 0x2B)      # SurfaceAlt, the lighter dark surface
LIGHT_TEXT_BG = (0xD1, 0xE6, 0xF7)     # AccentSoft, the palest light surface

# The loop in FitAccentForText: 20 passes, 10% per pass.
MAX_STEPS = 20
STEP_DIV = 10


def lin(c: float) -> float:
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def lum(rgb) -> float:
    # rgb is 0-255; lin() expects 0-1. Omitting the /255 makes lum(white) ~525000, which
    # pushes every ratio into the tens of thousands -- i.e. "always passes". Measured
    # before the fix: OnAccent reported a tightest margin of 99.0 (never set) instead of a
    # real number, and ratio(white,black) came out 10,498,937 instead of 21.
    r, g, b = rgb
    return (0.2126 * lin(r / 255.0)
            + 0.7152 * lin(g / 255.0)
            + 0.0722 * lin(b / 255.0))


def ratio(a, b) -> float:
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def fit_accent_for_text(accent, bg, towards_white: bool):
    """Mirror of FitAccentForText -- keep in sync with ThemeTokens.cpp."""
    r, g, b = accent
    for _ in range(MAX_STEPS):
        if ratio((r, g, b), bg) >= 4.5:
            break
        if towards_white:
            r += (255 - r) // STEP_DIV
            g += (255 - g) // STEP_DIV
            b += (255 - b) // STEP_DIV
        else:
            r = r * 9 // STEP_DIV
            g = g * 9 // STEP_DIV
            b = b * 9 // STEP_DIV
    return (r, g, b)


def on_accent(accent):
    """Mirror of the OnAccent rule: whichever of black/white scores higher."""
    l = lum(accent)
    cb = (l + 0.05) / 0.05
    cw = 1.05 / (l + 0.05)
    return (0, 0, 0) if cb >= cw else (255, 255, 255)


def sweep(hue_step=3, values=20, sats=(1.0, 0.85, 0.7, 0.55, 0.4, 0.25)):
    """Yield (hue, sat, val, accent_rgb) covering the space reasonably densely."""
    for h in range(0, 360, hue_step):
        for s in sats:
            for i in range(1, values + 1):
                v = i / float(values)
                r, g, b = colorsys.hsv_to_rgb(h / 360.0, s, v)
                yield h, s, v, (int(r * 255), int(g * 255), int(b * 255))


def main() -> int:
    failures = []
    counts = {"light": 0, "dark": 0, "onaccent": 0}
    tightest = {"light": (99.0, None), "dark": (99.0, None), "onaccent": (99.0, None)}

    for h, s, v, accent in sweep():
        for name, bg, tw in (("light", LIGHT_TEXT_BG, False),
                             ("dark", DARK_TEXT_BG, True)):
            fitted = fit_accent_for_text(accent, bg, tw)
            r = ratio(fitted, bg)
            counts[name] += 1
            if r < tightest[name][0]:
                tightest[name] = (r, (h, s, v, accent, fitted))
            if r < 4.5:
                failures.append((name, 4.5, r, h, s, v, accent, fitted))

        chosen = on_accent(accent)
        r = ratio(accent, chosen)
        counts["onaccent"] += 1
        if r < tightest["onaccent"][0]:
            tightest["onaccent"] = (r, (h, s, v, accent, chosen))
        if r < 4.5:
            failures.append(("OnAccent", 4.5, r, h, s, v, accent, chosen))

    print("[INFO] samples: light=%d dark=%d onaccent=%d"
          % (counts["light"], counts["dark"], counts["onaccent"]))
    for name in ("light", "dark", "onaccent"):
        r, detail = tightest[name]
        if detail:
            h, s, v, accent, out = detail
            print("       %-9s tightest %.2f at h=%3d s=%.2f v=%.2f accent=#%02X%02X%02X"
                  % (name, r, h, s, v, accent[0], accent[1], accent[2]))

    if failures:
        print("[FAIL] %d sample(s) below 4.5:1" % len(failures))
        for name, want, got, h, s, v, accent, out in failures[:10]:
            print("       %-9s h=%3d s=%.2f v=%.2f accent=#%02X%02X%02X -> %.2f (want %.1f)"
                  % (name, h, s, v, accent[0], accent[1], accent[2], got, want))
        return 1

    print("[PASS] accent-derived text clears 4.5:1 across the swept colour space")
    print("[NOTE] the sweep samples hue x sat x value on a grid; it is strong evidence,")
    print("       not a proof over every 24-bit colour.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
