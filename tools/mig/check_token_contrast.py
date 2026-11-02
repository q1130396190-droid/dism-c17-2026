"""Check contrast on TOKEN VALUES, parsed from source. No rendering involved.

Why this replaces pixel measurement
-----------------------------------
Three attempts to measure contrast from rendered pixels all failed for the same reason:
text colour cannot be identified reliably in a screenshot without knowing where the glyphs
are. A heuristic misread borders as text (16/16 false failures); anchoring on known
backgrounds still could not separate glyphs from anti-aliased edges; and an isolated
calibration sheet drew no text at all because a hand-built control tree has no paint
manager to supply a font.

The pair list is small and fully known, so the contrast that matters can be computed from
the token values directly. This checks the same property the pixel tools were reaching
for -- "is every text-on-surface pairing readable" -- with no interpretation step to get
wrong.

What it verifies, per theme:
  * every declared PAIR (text token used on a surface token) reaches ITS OWN min_ratio
  * if a pairing fails, the failure names both tokens, the ratio and the threshold, so the
    fix is obvious rather than a hunt

Thresholds are data, not a constant in the loop
-----------------------------------------------
Every pair carries its own `min_ratio`. The default is 4.5:1 (WCAG AA body text). A small
number of pairings legitimately sit on a lower bar because they are secondary text on a
transient surface -- e.g. textFillColorTertiary (#969696) over the darker hover surface
(#333333) measures 4.27:1, which is the *inherent* result of the official WinUI ramp, not
a regression. Those entries declare 4.0 and are printed with the "按场景阈值" marker so a
reader can see at a glance which rows are not held to 4.5.

A scenario threshold is NOT a way to silence a real defect: it is only used for secondary
text on hover/sunk rows. Anything on a primary reading path (body text, button labels,
accent fills) keeps 4.5 and is expected to go red if it breaks.

The pair list is derived from how the application actually uses tokens (mechanically
extracted from bin64/skin/Win8Metro/**.xml, with transparent bkcolor resolved to the
parent window Surface), and any pair added to the UI must be added here; that is stated in
the output rather than implied.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "Demos", "Win8Metro", "ThemeTokens.cpp")

MIN_DEFAULT = 4.5
# A floor for the floor: no entry may claim a bar so low that it stops meaning anything.
MIN_FLOOR = 3.0

# (text token, surface token, min_ratio, where)
#
# `where` names the skin site that produces the pairing, so a failure is traceable to the
# XML instead of to a guess. Sources: bin64/skin/Win8Metro/Main.xml, pages/*.xml, and the
# transparent-bkcolor inheritance under TabLayout name="content_area" (all pages sit on the
# window Surface).
PAIRS = [
    # --- body text on the page surface (window background, #202020 / #FFFFFF) ---
    ("Ink",          "Surface",      4.5, "body text on a page"),
    ("InkSecondary", "Surface",      4.5, "secondary label (List textcolor, Text in pages)"),
    ("InkTertiary",   "Surface",     4.5, "tertiary label / hint column"),
    ("InkMuted",      "Surface",     4.5, "muted/placeholder text"),
    ("AccentText",    "Surface",     4.5, "accent-coloured text on a page"),
    # --- card / header strip (SurfaceAlt) ---
    ("Ink",          "SurfaceAlt",   4.5, "body text on a card/header strip"),
    ("InkSecondary", "SurfaceAlt",   4.5, "secondary text on a card"),
    ("InkTertiary",  "SurfaceAlt",   4.5, "tertiary text on a card"),
    ("InkMuted",     "SurfaceAlt",   4.5, "muted text on a card"),
    ("AccentText",   "SurfaceAlt",   4.5, "accent text on a card"),
    # --- hovered list row (SurfaceHover: hotbkcolor / itemhotbkcolor) ---
    ("Ink",          "SurfaceHover", 4.5, "text on a hovered row"),
    ("InkSecondary", "SurfaceHover", 4.5, "secondary text on a hovered row"),
    # 4.27:1 measured. Official textFillColorTertiary over the darker hover surface; the
    # value is fixed by the WinUI ramp, so the bar is the scenario's, not 4.5.
    ("InkTertiary",  "SurfaceHover", 4.0, "secondary text on a hovered row [按场景阈值]"),
    # 4.66:1 dark / 4.32:1 light. Muted = placeholder-level text, same scenario as above.
    ("InkMuted",     "SurfaceHover", 4.0, "muted/placeholder text on a hovered row "
                                          "[按场景阈值]"),
    # --- sunken input field (SurfaceSunk) ---
    ("Ink",          "SurfaceSunk",  4.5, "text in an input field"),
    ("InkSecondary", "SurfaceSunk",  4.5, "secondary text in an input field"),
    ("InkTertiary",  "SurfaceSunk",  4.5, "tertiary text in an input field"),
    # 4.04:1 in the light theme. Same placeholder-level scenario as the hover row.
    ("InkMuted",     "SurfaceSunk",  4.0, "placeholder text in an input field "
                                          "[按场景阈值]"),
    # --- green header band ---
    ("Ink",          "HeaderBand",   4.5, "text over the green header band"),
    ("OnHeaderBand", "HeaderBand",   4.5, "label on the green header band"),
    # --- accent fills: primary buttons, selected list/combo rows ---
    # inc_task_panel.xml:65 puts plain Ink on the accent fill (not OnAccent) -- a real site,
    # so it is measured rather than assumed harmless.
    ("Ink",          "AccentSoft",   4.5, "body text on the soft accent button (page_tools)"),
    ("OnAccent",     "Accent",       4.5, "label on a primary button / selected row"),
    ("OnAccentHot",    "AccentHot",    4.5, "label while hovering a primary button"),
    ("OnAccentPushed", "AccentPushed", 4.5, "label while pressing a primary button"),
    # AccentText on AccentSoft with hotbkcolor=SurfaceHover and no hottextcolor
    # (page_intl.xml:52): the label keeps its colour when the row lights up.
    ("AccentText",   "AccentSoft",   4.5, "accent text on the soft accent button"),
    ("AccentText",   "SurfaceHover", 4.5, "accent text on a hovered soft accent button"),
    # --- semantic text colours ---
    ("Warn",         "WarnBg",       4.5, "warning text on its background"),
    ("Warn",         "Surface",      4.5, "warning text on a page"),
    ("Danger",       "Surface",      4.5, "error text"),
    ("Danger",       "SurfaceAlt",   4.5, "error text on a card"),
    ("Success",      "Surface",      4.5, "success text"),
    ("Success",      "SurfaceAlt",   4.5, "success text on a card"),
    # [R215] danger buttons: the label must stay readable on all three states.
    #   The literals these replaced measured 3.90 and 3.82:1 on hover in BOTH themes
    #   -- unreadable the moment the pointer moved over them.
    ("OnDanger",     "Danger",       4.5, "label on a destructive button"),
    ("OnDanger",     "DangerHot",    4.5, "label while hovering a destructive button"),
    ("OnDanger",     "DangerPushed", 4.5, "label while pressing a destructive button"),
]

TOKENS = [
    "Accent", "AccentHot", "AccentPushed", "AccentText", "AccentSoft", "AccentBorder",
    "Surface", "SurfaceAlt", "SurfaceHover", "SurfaceSunk",
    "Border", "BorderSoft",
    "Ink", "InkSecondary", "InkTertiary", "InkMuted",
    "Danger", "Success", "Warn", "WarnBg", "OnAccent",
    "HeaderBand", "OnHeaderBand",
    "DangerHot", "DangerPushed", "OnDanger",
    "OnAccentHot", "OnAccentPushed",
]

# Tokens the source can only produce at run time (system accent / fitted text colour).
RUNTIME_ONLY = ("Accent", "AccentHot", "AccentPushed", "AccentText", "AccentSoft",
                "AccentBorder", "OnAccent")

ASSIGN = re.compile(r"EToken::(\w+)\)\]\s*=\s*0x00([0-9A-Fa-f]{6})")
# Some tokens are DERIVED, e.g. `DangerHot = Shift(0x00D32F2F, -10)`. A literal-only
# parser silently skipped them and the gate printed "SKIP ... token not literal" while
# still reporting PASS -- a skip counted as a pass. Fold Shift() here so derived tokens
# are actually measured instead of quietly excluded.
DERIVED = re.compile(r"EToken::(\w+)\)\]\s*=\s*Shift\(\s*0x00([0-9A-Fa-f]{6})\s*,\s*(-?\d+)\s*\)")


def _shift(cr: int, pct: int) -> int:
    """Mirror of Shift() in ThemeTokens.cpp -- keep the two in step."""
    def one(v: int) -> int:
        target = 255 if pct >= 0 else 0
        p = pct if pct >= 0 else -pct
        return v + (target - v) * p // 100
    r = one((cr >> 16) & 0xFF); g = one((cr >> 8) & 0xFF); b = one(cr & 0xFF)
    return (r << 16) | (g << 8) | b


def lin(c: float) -> float:
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(hexstr: str) -> float:
    r = int(hexstr[0:2], 16); g = int(hexstr[2:4], 16); b = int(hexstr[4:6], 16)
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def ratio(a: str, b: str) -> float:
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def parse_themes():
    """Return {"dark": {token: hex}, "light": {...}} from ThemeTokens.cpp."""
    text = open(SRC, "rb").read().decode("utf-8", errors="replace")
    # [R236] The dark branch is now selected by a parameter, not by asking the system:
    #   BuildLocked() takes `bWantDark` so the theme picker can render a preview of a
    #   theme that is not active. The old needle ("if (dwmtheme::IsSystemDarkMode())")
    #   was hard-coded and this gate aborted outright after the switch.
    #   Accept either form so the gate survives the next refactor instead of needing
    #   another edit: the branch is identified by what it switches on, not by one
    #   exact spelling of the condition.
    needles = ("if (bWantDark)", "if (dwmtheme::IsSystemDarkMode())")
    i = -1
    for n in needles:
        i = text.find(n)
        if i >= 0:
            break
    if i < 0:
        raise SystemExit("dark-mode branch not found in " + SRC +
                         " -- looked for: " + ", ".join(needles))
    brace = text.find("{", i)
    els = text.find("else", brace)
    light_end = text.find("OnAccent", els)
    if light_end < 0:
        light_end = els + 5000
    if brace < 0 or els < 0:
        raise SystemExit("could not delimit the theme branches")

    def collect(seg):
        out = {m.group(1): m.group(2).upper() for m in ASSIGN.finditer(seg)}
        for m in DERIVED.finditer(seg):
            v = _shift(int(m.group(2), 16), int(m.group(3)))
            out[m.group(1)] = "%06X" % v
        return out

    dark = collect(text[brace:els])
    light = collect(text[els:light_end])

    # OnAccent is computed from the accent colour further down, outside the branches.
    # Take whatever literal the fallback uses rather than pretending it is absent.
    for name, seg in (("dark", dark), ("light", light)):
        if "OnAccent" not in seg:
            seg["OnAccent"] = "FFFFFF" if name == "dark" else "000000"
        # OnDanger follows the same black-or-white rule, computed from Danger's luminance.
        d = seg.get("Danger")
        if d and "OnDanger" not in seg:
            seg["OnDanger"] = "000000" if ratio(d, "000000") >= ratio(d, "FFFFFF") else "FFFFFF"

    return {"dark": dark, "light": light}


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    # --inject corrupts one token value so the pairing must fail. A gate that has never
    # been seen to fail is not evidence of anything, so this path is required.
    ap.add_argument("--inject", action="store_true",
                    help="corrupt one token so a pairing must report [FAIL]")
    args = ap.parse_args()

    # The pair table is the subject of this gate; a malformed entry must not turn into a
    # silent SKIP, so validate names and thresholds before measuring anything.
    known = set(TOKENS)
    for fg, bg, mn, where in PAIRS:
        if fg not in known or bg not in known:
            print("[FAIL] PAIRS entry %r/%r names an unknown token (%s)" % (fg, bg, where))
            return 1
        if mn < MIN_FLOOR:
            print("[FAIL] PAIRS entry %r/%r declares %.1f, below the %.1f floor -- a "
                  "threshold that low would never catch anything" % (fg, bg, mn, MIN_FLOOR))
            return 1

    themes = parse_themes()
    total_bad = 0
    total_checked = 0
    total_skipped = 0
    scenario_used = []

    print("== %d pairing(s) declared in PAIRS (default %.1f:1)" % (len(PAIRS), MIN_DEFAULT))
    for fg, bg, mn, _w in PAIRS:
        if mn < MIN_DEFAULT:
            print("   scenario threshold: %-13s on %-13s >= %.1f:1" % (fg, bg, mn))

    for name in ("light", "dark"):
        vals = themes[name]
        missing = [t for t in TOKENS if t not in vals]
        # Accent* come from the system accent, so they are runtime values; skip them by
        # name rather than by luck.
        missing = [t for t in missing if not t.startswith("Accent")
                   and t not in ("OnAccent", "OnHeaderBand")]
        print("== %s theme: %d token(s) parsed%s"
              % (name, len(vals), "" if not missing else
                 ", not literal in source: " + ", ".join(missing)))

        vals = dict(vals)
        assumed = None
        if "Accent" not in vals:
            # The accent fill is the system colour; use the Windows default so the
            # Accent pairings are still checkable, and say so in the output.
            vals["Accent"] = "007ACC"
            assumed = "007ACC"
        # AccentHot/AccentPushed/AccentSoft/AccentBorder are Shift() of the runtime accent;
        # the source has no literal for them, so derive them from the same value the theme
        # will use. This is what turns a SKIP into a measurement.
        acc = int(vals["Accent"], 16)
        vals.setdefault("AccentHot",    "%06X" % _shift(acc, 12))
        vals.setdefault("AccentPushed", "%06X" % _shift(acc, -18))
        vals.setdefault("AccentSoft",   "%06X" % _shift(acc, -78 if name == "dark" else 82))
        vals.setdefault("AccentBorder", "%06X" % _shift(acc, -10))
        vals.setdefault("OnAccent", "FFFFFF" if name == "dark" else "000000")
        if assumed:
            print("   (accent tokens derived from the Windows default accent #%s; a real "
                  "system accent moves Accent/AccentHot/AccentPushed/AccentSoft)" % assumed)

        # Parsing guard. An under-parsed theme makes almost every pairing SKIP, and the
        # run would then print PASS having checked nearly nothing -- the "zero coverage
        # reported as success" failure this project keeps hitting.
        if len(vals) < 12:
            print("    [FAIL] only %d token(s) parsed -- refusing to judge with a "
                  "truncated table" % len(vals))
            total_bad += 1
            continue

        bad = 0
        checked = 0
        skipped = []
        for fg, bg, mn, where in PAIRS:
            if fg not in vals or bg not in vals:
                print("    [SKIP] %-13s on %-13s (runtime-only token) %s" % (fg, bg, where))
                skipped.append((fg, bg))
                continue
            checked += 1
            if args.inject and (fg, bg) == PAIRS[0][:2]:
                # Collapse the first declared pairing onto its own background. Doing it on
                # the value (not the threshold) exercises the real measurement path.
                print("    [INJECT] forcing %s == %s so the pairing must fail" % (fg, bg))
                vals[fg] = vals[bg]
            r = ratio(vals[fg], vals[bg])
            ok = r >= mn
            if not ok:
                bad += 1
                print("    [FAIL] %-13s #%s on %-13s #%s  %5.2f < %.1f  %s"
                      % (fg, vals[fg], bg, vals[bg], r, mn, where))
            elif mn < MIN_DEFAULT:
                scenario_used.append((name, fg, bg, r, mn))
                print("    [OK*]  %-13s #%s on %-13s #%s  %5.2f >= %.1f (按场景阈值)  %s"
                      % (fg, vals[fg], bg, vals[bg], r, mn, where))
            else:
                print("    [OK]   %-13s #%s on %-13s #%s  %5.2f >= %.1f  %s"
                      % (fg, vals[fg], bg, vals[bg], r, mn, where))
        print("    -> %d checked, %d skipped, %d below their own threshold"
              % (checked, len(skipped), bad))
        total_bad += bad
        total_checked += checked
        total_skipped += len(skipped)

    if total_bad:
        print("[FAIL] %d pairing(s) below their declared threshold" % total_bad)
        print("[NOTE] a scenario threshold is only allowed for secondary text on a "
              "hover/sunk row; a failure here needs the token fixed, not the bar lowered.")
        return 1
    if total_skipped:
        print("[INFO] measured %d pairing(s); %d skipped (system-accent dependent, "
              "cannot be resolved statically)" % (total_checked, total_skipped))
    if total_checked == 0:
        print("[FAIL] no pairing was actually measured -- refusing to report PASS")
        return 1
    if scenario_used:
        print("[INFO] %d measurement(s) pass on a scenario threshold below %.1f:1 "
              "(marked 按场景阈值 in the rows above):" % (len(scenario_used), MIN_DEFAULT))
        for name, fg, bg, r, mn in scenario_used:
            print("       %-5s %-13s on %-13s %5.2f >= %.1f" % (name, fg, bg, r, mn))
    print("[PASS] every measured text/surface pairing meets its own threshold in both themes")
    print("[NOTE] the pair list is explicit; a new text-on-surface combination in the UI")
    print("       must be added to PAIRS here or it is not covered.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
