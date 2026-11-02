"""Fail when a text colour is hard-coded instead of taken from a theme token.

Why this gate exists
--------------------
Round 6 measured 25 hard-coded text colours across the application. Seventeen of them
were unreadable in the dark theme -- `#000000` on `#1F1F1F` is 1.27:1, `#2B2B2B` is 1.16:1.
None of them produced a warning; the text simply became invisible when the user switched
theme, which is the kind of defect no compile-time check catches.

The previous round "fixed" this by extending a lookup table inside FrameWnd that rewrote
known literals at runtime. That works only for values somebody remembered to add -- a new
`SetTextColor(0xFF123456)` is silently exempt. Fixing the values is not the same as
preventing new ones, so the rule is now mechanical: text colour must come from
tokens::Token() / tokens::TokenHex().

Why the allow-list is short and explicit
----------------------------------------
Some colours are legitimately fixed because they sit on a fixed-colour field:
white on an accent or danger fill does not need to follow the theme. Those are listed by
exact value with a reason, and the gate reports any entry that no longer matches anything
so the list cannot rot.

Exit codes: 0 clean, 1 findings, 2 input problem, 3 --inject failed to report.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "Demos", "Win8Metro")

# (regex, kind, description) -- every way this app can set a colour.
#   [R235] Each entry carries an explicit `kind`, because two argument shapes coexist:
#   "attr" -> group(1)=attribute name, group(2)=#AARRGGBB literal;
#   "args" -> group(1)=the whole argument list, every 0x literal inside is a colour.
#   The old code inferred the shape from len(m.groups()), so every new pattern was a
#   chance to silently misread a whole argument list as one colour value.
PATTERNS = [
    # [R234] Was "*textcolor" only, so bkcolor / hotbkcolor / pushedbkcolor /
    #   selectedbkcolor / itemhotbkcolor were never scanned. A tool dialog had
    #   `SetAttribute(L"hotbkcolor", L"#FFE81123")` and `pushedbkcolor="#FFE1EFF8"`
    #   right next to a tokenised textcolor -- the gate reported 0 findings.
    (re.compile(r'SetAttribute\(\s*L"(\w*(?:color)\w*)"\s*,\s*L"#([0-9A-Fa-f]{8})"\s*\)'),
     "attr", 'SetAttribute(L"*color*", L"#AARRGGBB")'),
    # [R224] Conditionals. The attr pattern needs the literal to be the ONLY argument,
    #   so `SetTextColor(bSel ? 0xFFFFFFFF : 0xFF1A1A1A)` was invisible -- and that is
    #   exactly where the round-23/24 leaks lived.
    # [R235] Generalised from `SetTextColor` to any `Set*Color`: SetBkColor,
    #   SetHotBkColor, SetPushedBkColor, SetSelectedBkColor, SetBorderColor all take a
    #   DWORD and were entirely unscanned. Round 234 had to find two of them by hand
    #   (a beige toolbar strip, a header fill) after this gate called the file clean.
    #   A generic name is deliberate: a future Set*Color must not start life exempt.
    #   GDI's `::SetTextColor(hdc, colour)` is a device-context call, so it is excluded.
    (re.compile(r"(?<!::)\bSet\w*Color\s*\(([^;]*)\)"),
     "args", "Set*Color(0xAARRGGBB)"),
]

# [R235] An argument list that also mentions a token is an alpha *composition*, not a
#   literal colour: `(0xF2000000 | (tokens::Token(SurfaceAlt) & 0x00FFFFFF))`. The RGB
#   already follows the theme; the literal is only the alpha. Failing these would push
#   people to drop transparency (a real visual regression) to get a green gate, so they
#   are counted and reported separately instead.
_TOKENS_IN_ARGS = re.compile(r"tokens::Token\w*\s*\(")
_HEX_IN_ARGS = re.compile(r"0x[0-9A-Fa-f]{8}")

# Values that are correct BECAUSE the field under them is a fixed colour.
# Keyed by 0xAARRGGBB, value = why it is allowed.
ALLOW = {
    0xFFFFFFFF: "white text on an accent/danger/header fill -- the fill itself is fixed",
    # [R235] Transparent is not a colour choice. XML_ALLOW already exempted it; the C++
    #   side did not, so widening the scanner to typed setters immediately flagged
    #   `SetBkColor(0x00000000)` -- a control deliberately left see-through.
    0x00000000: "fully transparent -- not a colour choice",
}

# Skin XML is where the round-14 leaks actually lived: four `bordercolor="#FFB8D0F0"`
# literals that the C++-only scan could never see (they measure 1.23:1 against the
# AccentSoft fill they outline -- effectively invisible, and wrong in the dark theme).
# Colour attributes that must follow the theme are checked here, not just textcolour.
XML_COLOUR_ATTRS = (
    "textcolor", "bordercolor", "bkcolor", "hotbkcolor", "pushedbkcolor",
    "selectedbkcolor", "hottextcolor", "pushedtextcolor", "selectedtextcolor",
    "linktextcolor", "disabledtextcolor", "disabledbkcolor",
)
# A fixed colour is legitimate on a fixed-colour field; same reasoning as ALLOW above.
XML_ALLOW = {
    "#FFFFFFFF": "white on an accent/danger/header fill",
    "#00000000": "fully transparent -- not a colour choice",
}

INJECT_FILE = "_zz_injected_textcolour.cpp"
# [R235] One planted line per pattern family, and the self-check asserts the *count*, not
#   just "something was reported". Before this, --inject planted a single SetTextColor and
#   passed even when the attribute / typed-setter patterns were broken, so extending the
#   scanner could never be proven by its own self-test.
INJECT_BODY = (
    '// planted by check_hardcoded_textcolor.py --inject\n'
    'void zz_probe(void* p)\n'
    '{\n'
    '    ((struct S*)p)->SetTextColor(0xFF123456);\n'                    # args
    '    ((struct S*)p)->SetBkColor(0xFF654321);\n'                      # args (typed setter)
    '    ((struct S*)p)->SetAttribute(L"hotbkcolor", L"#FFABCDEF");\n'   # attr
    '    ((struct S*)p)->SetBkColor(0xF2000000 | (tokens::Token(tokens::EToken::SurfaceAlt) & 0x00FFFFFF));\n'
    '}\n'
)
# The fourth line is an alpha composition: it must NOT be a finding, but it must still be
# recognised (as alpha-composed) or the mismatch below fires.
INJECT_EXPECT_FINDINGS = 3
INJECT_EXPECT_COMPOSED = 1


def scan_file(path):
    """Return ([(line, attr_desc, value, text)], alpha_composed, tokenised)."""
    with open(path, "rb") as fh:
        raw = fh.read()
    text = raw.decode("utf-8", errors="replace")
    out = []
    composed = 0
    tokenised = 0
    for lineno, line in enumerate(text.splitlines(), 1):
        for pat, kind, desc in PATTERNS:
            for m in pat.finditer(line):
                if kind == "attr":
                    value = int(m.group(2), 16)
                    attr = m.group(1)
                    if value in ALLOW:
                        continue
                    out.append((lineno, attr, value, line.strip()))
                    continue
                # kind == "args": read EVERY 0x literal, because a ternary has two
                # colours and catching only the first leaves the other unchecked.
                args = m.group(1)
                # [R235] Two different token cases, counted apart because they mean
                #   different things and the old single counter reported a true-but-
                #   useless number: 230 "alpha compositions" was almost entirely plain
                #   `SetBkColor(tokens::Token(...))` calls, i.e. the scanner's own
                #   normal path, not the exception worth reviewing.
                if _TOKENS_IN_ARGS.search(args):
                    if _HEX_IN_ARGS.search(args):
                        composed += 1      # alpha composition: still worth a human look
                    else:
                        tokenised += 1     # plain token call: the intended form
                    continue
                for lit in re.finditer(r"0x([0-9A-Fa-f]{8})", args):
                    value = int(lit.group(1), 16)
                    if value in ALLOW:
                        continue
                    out.append((lineno, desc, value, line.strip()))
    return out, composed, tokenised


def skin_files():
    base = os.path.join(ROOT, "bin64", "skin", "Win8Metro")
    return [os.path.join(base, "Main.xml")] + sorted(
        glob.glob(os.path.join(base, "pages", "*.xml")))


SKIN_INJECT = "_zz_injected_skin.xml"


def scan_skin(path):
    """Return [(line, attr, literal)] for hard-coded colours in skin XML."""
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8-sig", errors="replace")
    text = re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), text, flags=re.S)
    out = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for attr in XML_COLOUR_ATTRS:
            for m in re.finditer(r'\b%s="(#[0-9A-Fa-f]{8})"' % attr, line):
                val = m.group(1).upper()
                if val in XML_ALLOW:
                    continue
                out.append((lineno, attr, m.group(1)))
    return out


def app_files():
    files = []
    for ext in ("*.cpp", "*.h"):
        files += glob.glob(os.path.join(APP, ext))
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="plant a hard-coded text colour; the gate must report it")
    args = ap.parse_args()

    files = app_files()
    if not files:
        print("[FAIL] no application sources under " + APP)
        return 2

    # Zero-input guard: scanning nothing must never read as success.
    print("[INFO] files_scanned=%d" % len(files))

    planted = None
    if args.inject:
        planted = os.path.join(APP, INJECT_FILE)
        with open(planted, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(INJECT_BODY)
        # Scan the planted file alone, so the counts below are unambiguous: mixing it
        # with the real tree means one genuine finding makes a broken pattern look alive.
        plant_findings, plant_composed, _plant_tokenised = scan_file(planted)
        files = files + [planted]
    else:
        plant_findings, plant_composed, _plant_tokenised = [], 0, 0

    skins = skin_files()
    planted_skin = None
    if args.inject and skins:
        planted_skin = os.path.join(os.path.dirname(skins[0]), SKIN_INJECT)
        with open(planted_skin, "w", encoding="utf-8", newline="\n") as fh:
            fh.write('<Window><Button name="zz_injected" textcolor="#FF123456"/></Window>\n')
        skins = skins + [planted_skin]

    try:
        findings = []
        composed_alpha = 0
        tokenised = 0
        for p in files:
            file_findings, file_composed, file_tokenised = scan_file(p)
            composed_alpha += file_composed
            tokenised += file_tokenised
            for lineno, attr, value, text in file_findings:
                findings.append((os.path.relpath(p, ROOT), lineno, attr, value, text))
        skin_findings = []
        for p in skins:
            for lineno, attr, literal in scan_skin(p):
                skin_findings.append((os.path.relpath(p, ROOT), lineno, attr, literal))
    finally:
        if planted and os.path.exists(planted):
            os.remove(planted)
        if planted_skin and os.path.exists(planted_skin):
            os.remove(planted_skin)

    # tokenised is printed so a zero-finding run shows the scanner found the real code:
    #   "0 findings" with 0 tokenised calls would mean the widened patterns match nothing
    #   at all, which is indistinguishable from a broken regex unless the number is shown.
    print("[INFO] skin files=%d  skin colour attrs=%d  findings=%d"
          "  tokenised=%d  alpha-composed=%d"
          % (len(skins), len(XML_COLOUR_ATTRS), len(skin_findings),
             tokenised, composed_alpha))

    if args.inject:
        # [R235] Prove EVERY pattern family still fires, not just that something was found.
        #   Checking "findings > 0" would pass while the attribute or typed-setter pattern
        #   was broken, as long as one other pattern happened to work.
        print("[INFO] inject: findings=%d (expect %d), alpha-composed=%d (expect %d)"
              % (len(plant_findings), INJECT_EXPECT_FINDINGS,
                 plant_composed, INJECT_EXPECT_COMPOSED))
        if len(plant_findings) != INJECT_EXPECT_FINDINGS:
            print("[FAIL] inject: planted %d hard-coded colours but the scanner saw %d"
                  " -- at least one pattern family is blind"
                  % (INJECT_EXPECT_FINDINGS, len(plant_findings)))
            for lineno, attr, value, text in plant_findings:
                print("       got L%-5d #%08X  %s" % (lineno, value, attr))
            return 3
        if plant_composed != INJECT_EXPECT_COMPOSED:
            print("[FAIL] inject: alpha composition not recognised (%d, expect %d)"
                  " -- it would be reported as a hard-coded colour"
                  % (plant_composed, INJECT_EXPECT_COMPOSED))
            return 3

    if findings or skin_findings:
        print("[FAIL] %d hard-coded colour(s) -- these do not follow the theme:"
              % (len(findings) + len(skin_findings)))
        for rel, lineno, attr, value, text in findings[:25]:
            print("       %-52s L%-5d #%08X  %s" % (rel, lineno, value, attr))
        if len(findings) > 25:
            print("       ... and %d more" % (len(findings) - 25))
        for rel, lineno, attr, literal in skin_findings[:25]:
            print("       %-52s L%-5d %-10s %s" % (rel, lineno, literal, attr))
        if len(skin_findings) > 25:
            print("       ... and %d more" % (len(skin_findings) - 25))
        print("       use tokens::Token(EToken::Ink|InkTertiary|InkMuted|Danger|...)"
              " or tokens::TokenHex(...) so the colour tracks the theme")
        return 1

    print("[PASS] no hard-coded colour outside the allow-listed values "
          "(C++ allow=%d, skin allow=%d)" % (len(ALLOW), len(XML_ALLOW)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
